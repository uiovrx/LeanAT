import LeanAT.ModelIR.Schema

namespace LeanAT.SourceMapping

inductive OriginKind where
  | native | constructed | generated
  deriving Repr, BEq, Inhabited

structure SourceDocument where
  path : String
  content : String
  deriving Repr, BEq, Inhabited

structure NodeOrigin where
  nodePath : String
  documentPath : String
  startByte : Nat
  endByte : Nat
  kind : OriginKind := .native
  ancestry : List String := []
  deriving Repr, BEq, Inhabited

structure ModelNode where
  path : String
  kind : String
  text : String
  deriving Repr, BEq

partial def expressionNodes (path : String) (e : ModelIR.Expr) : List ModelNode :=
  let children : List ModelIR.Expr := match e with
    | .select _ c y n => [c,y,n] | .binary _ _ a b | .index _ a b => [a,b]
    | .field _ a _ | .unary _ _ a | .convert _ _ a | .variantTag _ a | .variantGet _ a _ _ => [a]
    | .compare _ _ a b => [a,b] | .vecSet _ a b c => [a,b,c]
    | .makeRecord _ es | .makeVariant _ _ es | .makeVec _ es | .callPure _ _ es => es | _ => []
  let kind := match e with
    | .literal .. => "literal" | .local .. => "local" | .state .. => "state"
    | .select .. => "select" | .binary .. => "binary" | .field .. => "field"
    | .index .. => "index" | .makeRecord .. => "makeRecord" | .makeVariant .. => "makeVariant"
    | .unary .. => "unary" | .compare .. => "compare" | .convert .. => "convert"
    | .variantTag .. => "variantTag" | .variantGet .. => "variantGet" | .makeVec .. => "makeVec"
    | .vecSet .. => "vecSet" | .callPure .. => "callPure"
  ⟨path,kind,reprStr e⟩ :: (children.zipIdx.flatMap fun (e,i) => expressionNodes s!"{path}/expr/{i}" e)

partial def statementNodes (nodePrefix : String) (stmts : List ModelIR.Stmt) : List ModelNode :=
  stmts.zipIdx.flatMap fun (stmt,i) =>
    let path := s!"{nodePrefix}/{i}"
    let expressions := match stmt with
      | .letVal _ e | .writeState _ e | .check e _ | .branch e _ _ | .await e _ _ | .transportReturn e => [e]
      | .emit _ es | .ret es | .serviceCall _ _ es => es | _ => []
    let kind := match stmt with
      | .letVal .. => "letVal" | .writeState .. => "writeState" | .check .. => "check"
      | .branch .. => "branch" | .repeat .. => "repeat" | .emit .. => "emit" | .fail .. => "fail"
      | .ret .. => "ret" | .unsupported .. => "unsupported" | .serviceCall .. => "serviceCall"
      | .await .. => "await" | .transportReturn .. => "transportReturn" | .readNow .. => "readNow"
    let nested := match stmt with
      | .branch _ yes no => statementNodes (path ++ "/yes") yes ++ statementNodes (path ++ "/no") no
      | .repeat _ body => statementNodes (path ++ "/body") body | _ => []
    ⟨path,kind,reprStr stmt⟩ :: (expressions.zipIdx.flatMap (fun (e,j) => expressionNodes s!"{path}/expr/{j}" e)) ++ nested

def handlerNodes (path : String) (h : ModelIR.Handler) : List ModelNode :=
  ⟨path,"handler",reprStr h⟩ :: statementNodes (path ++ "/body") h.body

/-- Preserve original ProcessIR identity and policies in the canonical source node. -/
def processNodes (path : String) (process : ModelIR.ProcessIR) : List ModelNode :=
  ⟨path,"process",reprStr process⟩ :: statementNodes (path ++ "/body") process.body


structure Bundle where
  documents : List SourceDocument := []
  origins : List NodeOrigin := []
  modelNodes : List (String × String) := []
  deriving Repr, BEq, Inhabited

def Bundle.rebase (bundle : Bundle) (nodePrefix : String) : Bundle :=
  { bundle with origins := bundle.origins.map (fun origin => { origin with nodePath := nodePrefix ++ origin.nodePath, ancestry := origin.ancestry.map (nodePrefix ++ ·) }), modelNodes := bundle.modelNodes.map (fun (path,text) => (nodePrefix ++ path,text)) }

def Bundle.merge (bundles : List Bundle) : Bundle :=
  { documents := (bundles.flatMap (·.documents)).eraseDups,
    origins := bundles.flatMap (·.origins),
    modelNodes := bundles.flatMap (·.modelNodes) }

/-- Exact snapshot binding for accidental stale-bundle detection; not a parser theorem. -/
def Bundle.bindHandlers (bundle : Bundle) (handlers : List ModelIR.Handler) : Bundle :=
  { bundle with modelNodes := handlers.flatMap fun handler =>
      (handlerNodes s!"handler/{handler.id}" handler).map fun node => (node.path,node.text) }

end LeanAT.SourceMapping

