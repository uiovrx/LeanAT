import Lean
import LeanAT.Reference.Contract

namespace LeanAT.Reference.JsonIO
open Lean

def maxDocumentBytes : Nat := 4 * 1024 * 1024
def maxItems : Nat := 65536

private def obj := Json.mkObj
private def decimal (n : Nat) : Json := .str (toString n)
private def array (xs : List α) (f : α → Json) : Json := .arr (xs.map f).toArray

def kindNumber : HandleKind → Nat
  | .transaction => 0 | .hop => 1 | .event => 2 | .process => 3 | .wait => 4
  | .result => 5 | .consumer => 6 | .scope => 7 | .task => 8 | .access => 9
  | .lease => 10 | .resourceTicket => 11 | .spawnTicket => 12 | .drain => 13 | .gateTicket => 14

private def parseKind : Nat → Except String HandleKind
  | 0 => pure .transaction | 1 => pure .hop | 2 => pure .event | 3 => pure .process
  | 4 => pure .wait | 5 => pure .result | 6 => pure .consumer | 7 => pure .scope
  | 8 => pure .task | 9 => pure .access | 10 => pure .lease | 11 => pure .resourceTicket
  | 12 => pure .spawnTicket | 13 => pure .drain | 14 => pure .gateTicket
  | _ => throw "JsonHandleKind"

def identityJson (h : HandleIdentity) : Json := obj [
  ("kind",toJson (kindNumber h.kind)),("domain",decimal h.domain.toNat),
  ("store",decimal h.store.toNat),("slot",decimal h.slot.toNat),
  ("generation",decimal h.generation.toNat),("owner",decimal h.owner.toNat)]

def valueJson : Value → Json
  | .unit => obj [("kind",.str "unit")]
  | .bool b => obj [("kind",.str "bool"),("value",.bool b)]
  | .bits w n => obj [("kind",.str "bits"),("width",toJson w),("value",decimal n)]
  | .bytes bs => obj [("kind",.str "bytes"),("data",array bs (fun b => toJson b.toNat))]
  | .record xs => obj [("kind",.str "record"),("fields",.arr (xs.map valueJson).toArray)]
  | .vec xs => obj [("kind",.str "vec"),("values",.arr (xs.map valueJson).toArray)]
  | .variant tag xs => obj [("kind",.str "variant"),("tag",toJson tag),("fields",.arr (xs.map valueJson).toArray)]
  | .handle h => obj [("kind",.str "handle"),("identity",identityJson h)]

private def fields (j : Json) (allowed : List String) : Except String Unit := do
  let o ← j.getObj?
  if o.toList.any (fun (k,_) => !allowed.contains k) then throw "JsonUnknownField"

private def natural (j : Json) (limit : Nat := 2^64-1) (requireString : Bool := false) : Except String Nat := do
  let n ← match j with
    | .str s => do
      if s.isEmpty || s.length > 20 || !(s.toList.all Char.isDigit) then throw "JsonUnsignedDecimal"
      let some n := s.toNat? | throw "JsonUnsignedDecimal"
      if toString n != s then throw "JsonNoncanonicalDecimal"
      pure n
    | .num n =>
      if requireString || n.exponent != 0 || n.mantissa < 0 then throw "JsonUnsignedDecimal"
      else pure n.mantissa.toNat
    | _ => throw "JsonUnsignedDecimal"
  if n > limit then throw "JsonIntegerRange"
  pure n

private def natField (j : Json) (key : String) (limit : Nat := 2^64-1) : Except String Nat := do
  natural (← j.getObjVal? key) limit
private def optional (j : Json) (key : String) (f : Json → Except String α) (fallback : α) : Except String α :=
  match j.getObjVal? key with | .ok v => f v | .error _ => pure fallback
private def optionalNat (j : Json) (key : String) (fallback : Nat) (limit : Nat := 2^64-1) :=
  optional j key (fun v => natural v limit) fallback
private def textValue (j : Json) : Except String String := do
  let s ← j.getStr?
  if s.utf8ByteSize > 65536 then throw "JsonStringBound"
  pure s
private def listOf (j : Json) (f : Json → Except String α) (limit : Nat := maxItems) : Except String (List α) := do
  let xs ← j.getArr?
  if xs.size > limit then throw "JsonArrayBound"
  xs.toList.mapM f

def identityOfJson (j : Json) : Except String HandleIdentity := do
  fields j ["kind","domain","store","slot","generation","owner"]
  let kind ← parseKind (← natField j "kind" 14)
  let domain ← natural (← j.getObjVal? "domain") (2^32-1) true
  let store ← natural (← j.getObjVal? "store") (2^32-1) true
  let slot ← natural (← j.getObjVal? "slot") (2^32-1) true
  let generation ← natural (← j.getObjVal? "generation") (2^64-1) true
  let owner ← natural (← j.getObjVal? "owner") (2^64-1) true
  pure ⟨kind,UInt32.ofNat domain,UInt32.ofNat store,UInt32.ofNat slot,UInt64.ofNat generation,UInt64.ofNat owner⟩

def valueOfJsonWithDepth : Nat → Json → Except String Value
  | 0, _ => throw "JsonValueDepth"
  | depth+1, j => do
    let kind ← (← j.getObjVal? "kind").getStr?
    match kind with
    | "unit" => fields j ["kind"]; pure .unit
    | "bool" => fields j ["kind","value"]; pure (.bool (← (← j.getObjVal? "value").getBool?))
    | "bits" =>
      fields j ["kind","width","value"]
      let width ← natField j "width" 64
      if width == 0 then throw "JsonBitsWidth"
      pure (.bits width (← natural (← j.getObjVal? "value") (2^width-1) true))
    | "bytes" =>
      fields j ["kind","data"]
      let bytes ← listOf (← j.getObjVal? "data") (fun b => UInt8.ofNat <$> natural b 255) 1048576
      pure (.bytes bytes)
    | "record" =>
      fields j ["kind","fields"]
      pure (.record (← listOf (← j.getObjVal? "fields") (valueOfJsonWithDepth depth)))
    | "vec" =>
      fields j ["kind","values"]
      pure (.vec (← listOf (← j.getObjVal? "values") (valueOfJsonWithDepth depth)))
    | "variant" =>
      fields j ["kind","tag","fields"]
      pure (.variant (← natField j "tag" (2^32-1)) (← listOf (← j.getObjVal? "fields") (valueOfJsonWithDepth depth)))
    | "handle" =>
      fields j ["kind","identity"]
      pure (.handle (← identityOfJson (← j.getObjVal? "identity")))
    | _ => throw "JsonValueKind"

def valueOfJson := valueOfJsonWithDepth 64
private def values (j : Json) := listOf j valueOfJson
private def optJson (f : α → Json) : Option α → Json | none => .null | some x => f x
private def optionOf (f : Json → Except String α) : Json → Except String (Option α)
  | .null => pure none | j => some <$> f j

def contextJson (c : Context) : Json := obj [
  ("kind",decimal c.kind),("now",decimal c.now),("turn",decimal c.turn),
  ("domain",decimal c.domain),("instanceId",decimal c.instanceId),("connection",decimal c.connection),("owner",decimal c.owner),
  ("processIdentity",optJson identityJson c.processIdentity),
  ("inputs",array c.inputs (fun (i,v) => obj [("index",decimal i),("value",valueJson v)])),
  ("environment",array c.environment (fun (n,v) => obj [("name",.str n),("value",valueJson v)]))]

def contextOfJson (j : Json) : Except String Context := do
  fields j ["kind","now","turn","domain","instanceId","connection","owner","processIdentity","inputs","environment"]
  let c : Context := {
    kind := ← optionalNat j "kind" 0 4
    now := ← optionalNat j "now" 0
    turn := ← optionalNat j "turn" 0
    domain := ← optionalNat j "domain" 0 (2^32-1)
    instanceId := ← optionalNat j "instanceId" 0 (2^32-1)
    connection := ← optionalNat j "connection" 0 (2^32-1)
    owner := ← optionalNat j "owner" 0
    processIdentity := ← optional j "processIdentity" (optionOf identityOfJson) none
    inputs := ← optional j "inputs" (fun v => listOf v (fun x => do
      fields x ["index","value"]
      pure (← natField x "index" (2^32-1),← valueOfJson (← x.getObjVal? "value")))) []
    environment := ← optional j "environment" (fun v => listOf v (fun x => do
      fields x ["name","value"]
      pure (← textValue (← x.getObjVal? "name"),← valueOfJson (← x.getObjVal? "value")))) [] }
  if (c.inputs.map Prod.fst).eraseDups.length != c.inputs.length ||
      (c.environment.map Prod.fst).eraseDups.length != c.environment.length then throw "JsonDuplicateBinding"
  checkContext c
  pure c

def objectJson (o : ObjectEntry) := obj [("identity",identityJson o.identity),("tag",.str o.tag),
  ("value",valueJson o.value),("alive",.bool o.alive)]
def objectOfJson (j : Json) : Except String ObjectEntry := do
  fields j ["identity","tag","value","alive"]
  pure {
    identity := ← identityOfJson (← j.getObjVal? "identity")
    tag := ← textValue (← j.getObjVal? "tag")
    value := ← valueOfJson (← j.getObjVal? "value")
    alive := ← optional j "alive" Json.getBool? true }

def eventJson (e : Event) := obj [("identity",identityJson e.identity),("time",decimal e.time),
  ("turn",decimal e.turn),("stage",decimal e.stage),("instanceId",decimal e.instanceId),
  ("connection",decimal e.connection),("sequence",decimal e.sequence),("kind",.str e.kind),
  ("values",array e.values valueJson),("source",.str e.source),("cancelled",.bool e.cancelled)]
def eventOfJson (j : Json) : Except String Event := do
  fields j ["identity","time","turn","stage","instanceId","connection","sequence","kind","values","source","cancelled"]
  pure {
    identity := ← identityOfJson (← j.getObjVal? "identity")
    time := ← natField j "time"
    turn := ← natField j "turn"
    stage := ← natField j "stage" 6
    instanceId := ← natField j "instanceId" (2^32-1)
    connection := ← natField j "connection" (2^32-1)
    sequence := ← natField j "sequence"
    kind := ← textValue (← j.getObjVal? "kind")
    values := ← optional j "values" values []
    source := ← optional j "source" textValue ""
    cancelled := ← optional j "cancelled" Json.getBool? false }

def observationJson (o : Observation) := obj [("kind",.str o.kind),("opcode",.str o.opcode),
  ("source",.str o.source),("time",decimal o.time),("turn",decimal o.turn),("values",array o.values valueJson)]
def observationOfJson (j : Json) : Except String Observation := do
  fields j ["kind","opcode","source","time","turn","values"]
  pure {
    kind := ← textValue (← j.getObjVal? "kind")
    opcode := ← optional j "opcode" textValue ""
    source := ← optional j "source" textValue ""
    time := ← optionalNat j "time" 0
    turn := ← optionalNat j "turn" 0
    values := ← optional j "values" values [] }

def allocationRuleJson (rule : AllocationRule) := obj [
  ("kind",toJson (kindNumber rule.kind)),("store",decimal rule.store),("group",.str rule.group),
  ("perSlot",.bool rule.perSlot),("persistent",.bool rule.persistent),("allowMax",.bool rule.allowMax),
  ("capacity",optJson decimal rule.capacity)]
def allocationRuleOfJson (j : Json) : Except String AllocationRule := do
  fields j ["kind","store","group","perSlot","persistent","allowMax","capacity"]
  pure {
    kind := ← parseKind (← natField j "kind" 14)
    store := ← natField j "store" (2^32-1)
    group := ← textValue (← j.getObjVal? "group")
    perSlot := ← optional j "perSlot" Json.getBool? false
    persistent := ← optional j "persistent" Json.getBool? true
    allowMax := ← optional j "allowMax" Json.getBool? true
    capacity := ← optional j "capacity" (optionOf (fun v => natural v)) none }

def allocationCounterJson (counter : AllocationCounter) := obj [
  ("group",.str counter.group),("domain",decimal counter.domain),("slot",optJson decimal counter.slot),
  ("nextGeneration",decimal counter.nextGeneration),("persistent",.bool counter.persistent),
  ("retired",.bool counter.retired)]
def allocationCounterOfJson (j : Json) : Except String AllocationCounter := do
  fields j ["group","domain","slot","nextGeneration","persistent","retired"]
  pure {
    group := ← textValue (← j.getObjVal? "group")
    domain := ← natField j "domain" (2^32-1)
    slot := ← optional j "slot" (optionOf (fun v => natural v (2^32-1))) none
    nextGeneration := ← optionalNat j "nextGeneration" 1 (2^64)
    persistent := ← optional j "persistent" Json.getBool? true
    retired := ← optional j "retired" Json.getBool? false }

def stateJson (s : State) := obj [("objects",array s.objects objectJson),("events",array s.events eventJson),
  ("allocationRules",array s.allocationRules allocationRuleJson),("allocationCounters",array s.allocationCounters allocationCounterJson),
  ("observations",array s.observations observationJson),("nextGeneration",decimal s.nextGeneration),
  ("generationLimit",decimal s.generationLimit),("nextSequence",decimal s.nextSequence),
  ("maxObjects",decimal s.maxObjects),("maxPins",decimal s.maxPins),("maxEvents",decimal s.maxEvents),("maxBytes",decimal s.maxBytes),
  ("maxObservations",decimal s.maxObservations)]
def stateOfJson (j : Json) : Except String State := do
  fields j ["objects","events","observations","allocationRules","allocationCounters","nextGeneration","generationLimit","nextSequence",
    "maxObjects","maxPins","maxEvents","maxBytes","maxObservations"]
  let s : State := {
    objects := ← optional j "objects" (fun v => listOf v objectOfJson) []
    events := ← optional j "events" (fun v => listOf v eventOfJson) []
    observations := ← optional j "observations" (fun v => listOf v observationOfJson 100000) []
    allocationRules := ← optional j "allocationRules" (fun v => listOf v allocationRuleOfJson) []
    allocationCounters := ← optional j "allocationCounters" (fun v => listOf v allocationCounterOfJson) []
    nextGeneration := ← optionalNat j "nextGeneration" 1 (2^64)
    generationLimit := ← optionalNat j "generationLimit" (2^64) (2^64)
    nextSequence := ← optionalNat j "nextSequence" 1
    maxObjects := ← optionalNat j "maxObjects" 1024
    maxPins := ← optionalNat j "maxPins" 256
    maxEvents := ← optionalNat j "maxEvents" 1024
    maxBytes := ← optionalNat j "maxBytes" 1048576
    maxObservations := ← optionalNat j "maxObservations" 100000 }
  validateState s
  pure s

def traceJson (t : ExecutionTrace) := obj [("layer",.str t.layer),("program",decimal t.program),
  ("location",.str t.location),("operation",.str t.operation),("args",array t.args valueJson),
  ("results",array t.results valueJson),("fuelBefore",decimal t.fuelBefore),("fuelAfter",decimal t.fuelAfter)]
def outcomeJson (o : Outcome) := obj [("schema",.str "leanat.reference-outcome.v1"),
  ("ok",.bool o.ok),("error",.str o.error),("returned",array o.returned valueJson),
  ("committed",array o.committed valueJson),("world",stateJson o.world),("remainingFuel",decimal o.remainingFuel),
  ("runtimeFuelRemaining",optJson decimal o.runtimeFuelRemaining),
  ("exit",.str o.exit),("wait",optJson valueJson o.wait),("resumeBlock",optJson decimal o.resumeBlock),
  ("outcomeType",optJson decimal o.outcomeType),("live",array o.live valueJson),("trace",array o.trace traceJson)]

structure Input where
  inputs : List Value := []
  committed : List Value := []
  world : State := {}
  context : Context := {}
  fuel : Nat := 10000
  deriving Repr, BEq

def inputJson (i : Input) := obj [("schema",.str "leanat.reference-input.v1"),
  ("inputs",array i.inputs valueJson),("committed",array i.committed valueJson),
  ("world",stateJson i.world),("context",contextJson i.context),("fuel",decimal i.fuel)]
def inputOfJson (j : Json) : Except String Input := do
  fields j ["schema","inputs","committed","world","context","fuel"]
  if (← (← j.getObjVal? "schema").getStr?) != "leanat.reference-input.v1" then throw "JsonInputSchema"
  pure {
    inputs := ← optional j "inputs" values []
    committed := ← optional j "committed" values []
    world := ← optional j "world" stateOfJson {}
    context := ← optional j "context" contextOfJson {}
    fuel := ← optionalNat j "fuel" 10000 }

/-- Preflight bounds parser recursion and numeric tokens before the general JSON parser.
    JSON numbers in this wire format are unsigned integer metadata only. -/
def parseBounded (source : String) : Except String Json := do
  if source.utf8ByteSize > maxDocumentBytes then throw "JsonDocumentBound"
  let mut depth := 0
  let mut quoted := false
  let mut escaped := false
  let mut token := 0
  let mut numericToken := false
  let mut keyChars : List Char := []
  let mut pendingString : Option String := none
  let mut objectKeys : List (Bool × List String) := []
  for c in source.toList do
    if quoted then
      keyChars := c :: keyChars
      if escaped then escaped := false
      else if c == '\\' then escaped := true
      else if c == '"' then
        quoted := false
        pendingString := some (String.ofList keyChars.reverse)
        keyChars := []
    else if c == '"' then
      quoted := true
      token := 0
      keyChars := ['"']
    else if c == '[' || c == '{' then
      depth := depth+1
      token := 0
      objectKeys := (c == '{',[]) :: objectKeys
      if depth > 160 then throw "JsonDocumentDepth"
    else if c == ']' || c == '}' then
      if depth == 0 then throw "JsonDocumentBrackets"
      depth := depth-1
      token := 0
      objectKeys := objectKeys.tail
      pendingString := none
    else if c == ':' then
      let some raw := pendingString | throw "JsonObjectKey"
      let key ← (← Json.parse raw).getStr?
      let (true,keys) :: rest := objectKeys | throw "JsonObjectKey"
      if keys.length ≥ 128 then throw "JsonObjectFieldBound"
      if keys.contains key then throw "JsonDuplicateField"
      objectKeys := (true,key :: keys) :: rest
      pendingString := none
      token := 0
    else if c.isWhitespace || c == ',' then token := 0
    else
      if token == 0 then numericToken := c.isDigit
      token := token+1
      if token > 20 || c == '+' || c == '-' || c == '.' || c == 'E' ||
          (numericToken && !c.isDigit) then throw "JsonNumericToken"
  if quoted || depth != 0 then throw "JsonDocumentBrackets"
  Json.parse source

def readInput (path : System.FilePath) : IO Input := do
  let handle ← IO.FS.Handle.mk path .read
  let mut bytes := ByteArray.empty
  while bytes.size ≤ maxDocumentBytes do
    let chunk ← handle.read (USize.ofNat (maxDocumentBytes+1-bytes.size))
    if chunk.isEmpty then break
    bytes := bytes ++ chunk
  if bytes.size > maxDocumentBytes then throw (IO.userError "JsonDocumentBound")
  let some source := String.fromUTF8? bytes | throw (IO.userError "JsonInvalidUTF8")
  match parseBounded source >>= inputOfJson with
  | .ok input => pure input
  | .error message => throw (IO.userError message)

def writeOutcome (path : System.FilePath) (outcome : Outcome) : IO Unit :=
  IO.FS.writeFile path ((outcomeJson outcome).compress ++ "\n")

end LeanAT.Reference.JsonIO
